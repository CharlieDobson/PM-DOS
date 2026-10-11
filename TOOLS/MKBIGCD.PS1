# MKBIGCD.PS1 - build S:\Software\PMDOSCD\BIGDIR.ISO, a disc with one
# directory of eight thousand files on it.
#
# WHY A DISC HAD TO BE MADE.  The discs already here list quickly on
# any kernel: the largest directory on win16ddk.iso holds 347 names.
# The CD-ROM reader found a directory's record N by counting from the
# front, once for every record asked for, so a listing cost the square
# of what was in the directory and nothing here was big enough to show
# it.  DOSSHELL reading a disc like this one stood at "Directories
# read: 96" for two minutes on the 86Box Pentium Pro; see iso_curget
# in DOS\ISO9660.INC.
#
#   A001 ... A100          a hundred small directories first, so the
#                          shell's count has somewhere to stop: every
#                          tenth has SUB1 and SUB2 in it
#   BIG                    FILE0001.TXT to FILE8000.TXT, each holding
#                          its own number, and then SUB1 to SUB5 - five
#                          names looked up behind eight thousand others
#   Z001 ... Z030          and what comes after it
#
# -Files makes BIG smaller.  1500 is a disc the kernel before the
# change gets through, which is what a comparison of the two needs.
# -Fs 3 is ISO9660 + Joliet, as TOOLS\MKISO.PS1 makes; 1 is ISO9660
# alone and 4 is UDF alone, which is the other reader (udf_curget).
param([string]$Out  = 'S:\Software\PMDOSCD\BIGDIR.ISO',
      [string]$Work = "$env:TEMP\pmdos-bigsrc",
      [int]   $Files = 8000,
      [int]   $Fs = 3)

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
public class BigWriter {
  public static void Create(string path, object stream, int blockSize, int totalBlocks) {
    IStream i = (IStream)stream;
    byte[] buf = new byte[blockSize];
    IntPtr pcb = Marshal.AllocHGlobal(4);
    using (FileStream o = File.Create(path)) {
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

function Put( [string]$path, [string]$text ) { [IO.File]::WriteAllText( $path, $text, [Text.Encoding]::ASCII ) }

Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Work | Out-Null
foreach ( $index in 1..100 ) {
    $dir = '{0}\A{1:000}' -f $Work, $index
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ( $file in 1..3 ) { Put "$dir\READ$file.TXT" "a$index $file" }
    if ( $index % 10 -eq 0 ) {
        New-Item -ItemType Directory -Force "$dir\SUB1", "$dir\SUB2" | Out-Null
        Put "$dir\SUB1\X.TXT" 'x'
    }
}
New-Item -ItemType Directory -Force "$Work\BIG" | Out-Null
foreach ( $index in 1..$Files ) { Put ( '{0}\BIG\FILE{1:0000}.TXT' -f $Work, $index ) "f$index" }
foreach ( $index in 1..5 ) {
    New-Item -ItemType Directory -Force "$Work\BIG\SUB$index" | Out-Null
    Put "$Work\BIG\SUB$index\S.TXT" 's'
}
foreach ( $index in 1..30 ) {
    $dir = '{0}\Z{1:000}' -f $Work, $index
    New-Item -ItemType Directory -Force $dir | Out-Null
    Put "$dir\Z.TXT" "z$index"
}

Remove-Item $Out -ErrorAction SilentlyContinue
$fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
$fsi.FileSystemsToCreate = $Fs
if ( $Fs -band 4 ) { $fsi.UDFRevision = 0x102 }
$fsi.VolumeName = 'BIGDIR'
$fsi.Root.AddTree($Work, $false)
$r = $fsi.CreateResultImage()
[BigWriter]::Create($Out, $r.ImageStream, $r.BlockSize, $r.TotalBlocks)
Get-Item $Out | Select-Object FullName, Length
