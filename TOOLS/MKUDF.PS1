# MKUDF.PS1 - build S:\Software\PMDOSCD\UDFTEST.ISO, a disc with UDF on
# it and NO ISO9660 BRIDGE, which is the only kind of disc the UDF
# reader is for.
#
# WHY THIS AND NOT MKCD.PL.  Everything else the CD-ROM work needed was
# written by hand in TOOLS\MKCD.PL, because no tool on this machine
# could make a Rock Ridge disc, a second session or an audio track.
# UDF is the opposite case: IMAPI2FS - Windows' own image builder, the
# thing that burns a disc from Explorer - writes it properly, including
# the descriptor CRCs that a hand-written writer would have to get
# right for no benefit to anybody.  Using it also makes the disc an
# INDEPENDENT article: it is not written by the same person who wrote
# the reader, which is the whole argument for an oracle.
#
#   FileSystemsToCreate = 4        UDF only.  1 is ISO9660 and 2 is
#                                  Joliet; a disc with any of those on
#                                  it would be mounted through the
#                                  ISO9660 path and would test nothing
#   UDFRevision = 0x102            UDF 1.02, which is what a DVD-Video
#                                  disc carries and what every reader
#                                  of the period understood
#
# THE ORACLE IS WINDOWS ITSELF: Mount-DiskImage on the result gives a
# drive letter whose listing can be compared with what PM-DOS shows,
# and 7-Zip reads UDF as well.  What PM-DOS must show is the same
# names, the same sizes and the same tree.
param([string]$Out  = 'S:\Software\PMDOSCD\UDFTEST.ISO',
      [string]$Work = "$env:TEMP\pmdos-udfsrc",
      [int]   $Rev  = 0x102)

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
public class UdfWriter {
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

# The files are chosen to exercise what the reader has branches for,
# and every one of them says what it is so that a TYPE on the guest is
# a test rather than a read.
Set-Content -NoNewline -Path "$Work\README.TXT" -Value @"
This disc has UDF on it and no ISO9660 at all.  Block 16 says BEA01
instead of CD001, so a reader that only knows ISO9660 cannot mount it
and should say so rather than showing rubbish.
"@
Set-Content -NoNewline -Path "$Work\A Long UDF Name With Spaces.txt" -Value @"
A name that needs the long-name path, spelled in UDF's own d-string
form rather than in Joliet's UCS-2 or Rock Ridge's NM entry.
"@
Set-Content -NoNewline -Path "$Work\lower.txt" -Value @"
Lower case only: the 8.3 alias differs from the name in case alone and
must not get a tilde.
"@
Set-Content -NoNewline -Path "$Work\cafe menu.txt" -Value @"
A plain name beside the accented one, so the two can be compared.
"@
Set-Content -NoNewline -Path "$Work\caf$([char]0xE9) men$([char]0xFC).txt" -Value @"
Two characters codepage 437 can spell, in a UDF d-string.
"@
New-Item -ItemType Directory -Force "$Work\Sub Directory" | Out-Null
Set-Content -NoNewline -Path "$Work\Sub Directory\inner.txt" -Value @"
One level down.  Walking in here needs the subdirectory's File Entry
to be read for its extent and its length, which is the one thing UDF
does not put in the directory record itself.
"@
New-Item -ItemType Directory -Force "$Work\Sub Directory\Deeper" | Out-Null
Set-Content -NoNewline -Path "$Work\Sub Directory\Deeper\deep.txt" -Value @"
Two levels down.
"@

# A FILE OF SEVERAL BLOCKS, because a one-block file is read correctly
# by a reader that ignores the extent length entirely.
$big = New-Object System.Text.StringBuilder
[void]$big.AppendLine('BIG.TXT, first line.  The rest is filler so that this file needs')
[void]$big.AppendLine('several blocks, and the last line says so.')
for ($i = 0; $i -lt 200; $i++) { [void]$big.AppendLine(('.' * 70)) }
[void]$big.AppendLine('BIG.TXT, LAST LINE - and reaching it means every block was read.')
Set-Content -NoNewline -Path "$Work\BIG.TXT" -Value $big.ToString()

Remove-Item $Out -ErrorAction SilentlyContinue
$fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
$fsi.FileSystemsToCreate = 4            # UDF, and nothing else
$fsi.UDFRevision = $Rev
$fsi.VolumeName = 'UDFTEST'
$fsi.Root.AddTree($Work, $false)
$r = $fsi.CreateResultImage()
[UdfWriter]::Create($Out, $r.ImageStream, $r.BlockSize, $r.TotalBlocks)
Get-Item $Out | Select-Object FullName, Length
