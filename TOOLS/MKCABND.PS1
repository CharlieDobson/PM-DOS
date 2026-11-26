# ===================================================================
# MKCABND.PS1 - build DISK\CACERT.BND, the CA trust bundle PM-DOS
# ships for HTTPS certificate verification.
#
# The bundle is a plain concatenation of DER root certificates - the
# exact form CMD\NET\TLS\TLSHS.C's ca_next() walks.  Each root is named
# by thumbprint and read from the Windows certificate store.  Windows
# adds roots to its store only when something needs them, so a root
# this machine has not met comes from the Windows Update root list
# instead (certutil -generateSSTFromWU).  The thumbprint fixes the
# content either way.
#
#   powershell -ExecutionPolicy Bypass -File TOOLS\MKCABND.PS1
#   powershell ... -File TOOLS\MKCABND.PS1 -Out somewhere\CACERT.BND
#
# A listed root that cannot be found, or that has expired, stops the
# script and the existing bundle is left as it was.
#
# TLSHS.C walks up from the server's certificate and stops at the first
# one signed by a bundle root that is in date, so a root is needed only
# if some certificate servers send is signed by it directly.  Before
# removing a root, or adding one, look at the chains real servers send
# (openssl s_client -showcerts).
#
# Only roots whose public key NSTLS can use are listed: RSA, or EC on
# P-256 / P-384.  A root the port cannot parse would simply be skipped
# at verify time, so a stray one is harmless, but the list is kept to
# keys the port understands on purpose.  Add a root by putting its
# thumbprint below; find it with:
#
#   Get-ChildItem Cert:\LocalMachine\Root | Format-List Subject,Thumbprint
# ===================================================================
param(
    [string]$Out = ( Join-Path $PSScriptRoot '..\DISK\CACERT.BND' )
)
$ErrorActionPreference = 'Stop'

# name  ::  thumbprint - the order is the order they are tried at verify
# time, so the roots that anchor the most servers come first.
$roots = @(
    @{ n = 'USERTrust RSA Certification Authority';             t = '2B8F1B57330DBBA2D07A6C51F70EE90DDAB9AD8E' },
    @{ n = 'USERTrust ECC Certification Authority';              t = 'D1CBCA5DB2D52A7F693B674DE5F05A1D0C957DF0' },
    @{ n = 'Sectigo Public Server Authentication Root R46';      t = 'AD98F9F3E47D753B65D482B3A45217BB6EF5E438' },
    @{ n = 'ISRG Root X1 (Lets Encrypt)';                        t = 'CABD2A79A1076A31F21D253635CB039D4329A5E8' },
    @{ n = 'ISRG Root X2 (Lets Encrypt, ECC P-384)';             t = 'BDB1B93CD5978D45C6261455F8DB95C75AD153AF' },
    @{ n = 'DigiCert Global Root CA';                            t = 'A8985D3A65E5E5C4B2D7D66D40C6DD2FB19C5436' },
    @{ n = 'DigiCert Global Root G2';                            t = 'DF3C24F9BFD666761B268073FE06D1CC8D4F82A4' },
    @{ n = 'DigiCert Global Root G3 (ECC P-384)';                t = '7E04DE896A3E666D00E687D33FFAD93BE83D349E' },
    @{ n = 'DigiCert High Assurance EV Root CA';                 t = '5FB7EE0633E259DBAD0C4C9AE6D38F1A61C7DC25' },
    @{ n = 'DigiCert Trusted Root G4';                           t = 'DDFB16CD4931C973A2037D3FC83A4D7D775D05E4' },
    @{ n = 'DigiCert Assured ID Root CA';                        t = '0563B8630D62D75ABBC8AB1E4BDFB5A899B24D43' },
    @{ n = 'GlobalSign Root CA (R1, ends 2028-01-28)';           t = 'B1BC968BD4F49D622AA89A81F2150152A41D829C' },
    @{ n = 'GlobalSign Root CA - R3';                            t = 'D69B561148F01C77C54578C10926DF5B856976AD' },
    @{ n = 'GlobalSign Root R46';                                t = '53A2B04BCA6BD645E6398A8EC40DD2BF77C3A290' },
    @{ n = 'GTS Root R1 (Google)';                               t = 'E1C950E6EF22F84C5645728B922060D7D5A7A3E8' },
    @{ n = 'GTS Root R4 (Google, ECC P-384)';                    t = '77D30367B5E00C15F60C3861DF7CE13B92464D47' },
    @{ n = 'Microsoft RSA Root Certificate Authority 2017';      t = '73A5E64A3BFF8316FF0EDCCC618A906E4EAE4D74' },
    @{ n = 'Microsoft ECC Root Certificate Authority 2017';      t = '999A64C37FF47D9FAB95F14769891460EEC4C3C5' },
    @{ n = 'Go Daddy Root Certificate Authority - G2';           t = '47BEABC922EAE80E78783462A79F45C254FDE68B' },
    @{ n = 'Starfield Root Certificate Authority - G2';          t = 'B51C067CEE2B0C3DF855AB2D92F4FE39D4E70F0E' },
    @{ n = 'Starfield Services Root Certificate Authority - G2'; t = '925A8F8D2C6D04E0665F596AFF22D863E8256F3F' },
    @{ n = 'Amazon Root CA 1';                                   t = '8DA7F965EC5EFC37910F1C6E59FDC1CC6A6EDE16' },
    @{ n = 'SSL.com TLS ECC Root CA 2022';                       t = '9F5FD91A546DF50C71F0EE7ABD1749988473E239' },
    @{ n = 'Entrust Root Certification Authority - G2';          t = '8CF427FD790C3AD166068DE81E57EFBB932272D4' },
    @{ n = 'Certum Trusted Network CA';                          t = '07E032E020B72C3F192F0628A2593A19A70F069E' },
    @{ n = 'IdenTrust Commercial Root CA 1';                     t = 'DF717EAA4AD94EC9558499602D48DE5FBCF03A25' }
)

$wuRoots = $null

# the Windows Update root list, fetched once and only if a root needs it
function Get-WuRoots {
    if ( $null -ne $script:wuRoots ) { return }
    $dir = Join-Path ( [IO.Path]::GetTempPath() ) ( 'MKCABND-' + [Guid]::NewGuid().ToString( 'N' ) )
    New-Item -ItemType Directory -Path $dir | Out-Null
    try {
        $sst = Join-Path $dir 'ROOTS.SST'
        & certutil.exe -f -generateSSTFromWU $sst | Out-Null
        if ( $LASTEXITCODE -ne 0 ) { throw 'certutil could not fetch the Windows Update root list' }
        $list = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2Collection
        $list.Import( $sst )
        $script:wuRoots = $list
    } finally {
        Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Find-Cert( $thumb ) {
    foreach ( $store in @( 'LocalMachine', 'CurrentUser' ) ) {
        $hit = Get-ChildItem "Cert:\$store\Root" | Where-Object { $_.Thumbprint -eq $thumb }
        if ( $hit ) { return $hit[0] }
    }
    Get-WuRoots
    foreach ( $listed in $script:wuRoots ) {
        if ( $listed.Thumbprint -eq $thumb ) { return $listed }
    }
    return $null
}

$bytes = New-Object System.Collections.Generic.List[byte]
$found = 0
$bad   = 0
$now   = Get-Date
foreach ( $root in $roots ) {
    $cert = Find-Cert $root.t
    if ( $null -eq $cert ) {
        $bad++
        Write-Output ( "  MISSING             {0}" -f $root.n )
        continue
    }
    if ( $cert.NotAfter -lt $now ) {
        $bad++
        Write-Output ( "  EXPIRED {0:yyyy-MM-dd}  {1}" -f $cert.NotAfter, $root.n )
        continue
    }
    $der = $cert.RawData
    $bytes.AddRange( $der )
    $found++
    Write-Output ( "  {0,6}  {1:yyyy-MM-dd}  {2}" -f $der.Length, $cert.NotAfter, $root.n )
}

$full = [IO.Path]::GetFullPath( $Out )
if ( $bad -gt 0 ) {
    throw ( "{0} root(s) missing or expired; {1} not written" -f $bad, $full )
}
[IO.File]::WriteAllBytes( $full, $bytes.ToArray() )

Write-Output ''
Write-Output ( "{0} roots, {1} bytes -> {2}" -f $found, $bytes.Count, $full )
