# ===================================================================
# MKCABND.PS1 - build DISK\CACERT.BND, the CA trust bundle PM-DOS
# ships for HTTPS certificate verification.
#
# The bundle is a plain concatenation of DER root certificates - the
# exact form CMD\NET\TLS\TLSHS.C's ca_next() walks.  Each root is read
# from the Windows certificate store by thumbprint, so the file is
# reproducible on this machine without a download: the roots are the
# ones Windows already trusts.
#
#   powershell -ExecutionPolicy Bypass -File TOOLS\MKCABND.PS1
#   powershell ... -File TOOLS\MKCABND.PS1 -Out somewhere\CACERT.BND
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
    @{ n = 'AAA Certificate Services (Sectigo/Comodo)';          t = 'D1EB23A46D17D68FD92564C2F1F1601764D8E349' },
    @{ n = 'USERTrust RSA Certification Authority';              t = '2B8F1B57330DBBA2D07A6C51F70EE90DDAB9AD8E' },
    @{ n = 'USERTrust ECC Certification Authority';              t = 'D1CBCA5DB2D52A7F693B674DE5F05A1D0C957DF0' },
    @{ n = 'Sectigo Public Server Authentication Root R46';      t = 'AD98F9F3E47D753B65D482B3A45217BB6EF5E438' },
    @{ n = 'ISRG Root X1 (Lets Encrypt)';                        t = 'CABD2A79A1076A31F21D253635CB039D4329A5E8' },
    @{ n = 'DigiCert Global Root CA';                            t = 'A8985D3A65E5E5C4B2D7D66D40C6DD2FB19C5436' },
    @{ n = 'DigiCert Global Root G2';                            t = 'DF3C24F9BFD666761B268073FE06D1CC8D4F82A4' },
    @{ n = 'DigiCert Global Root G3 (ECC P-384)';                t = '7E04DE896A3E666D00E687D33FFAD93BE83D349E' },
    @{ n = 'DigiCert High Assurance EV Root CA';                 t = '5FB7EE0633E259DBAD0C4C9AE6D38F1A61C7DC25' },
    @{ n = 'DigiCert Trusted Root G4';                           t = 'DDFB16CD4931C973A2037D3FC83A4D7D775D05E4' },
    @{ n = 'DigiCert Assured ID Root CA';                        t = '0563B8630D62D75ABBC8AB1E4BDFB5A899B24D43' },
    @{ n = 'Baltimore CyberTrust Root';                          t = 'D4DE20D05E66FC53FE1A50882C78DB2852CAE474' },
    @{ n = 'GlobalSign Root CA (R1)';                            t = 'B1BC968BD4F49D622AA89A81F2150152A41D829C' },
    @{ n = 'GlobalSign Root CA - R2';                            t = '75E0ABB6138512271C04F85FDDDE38E4B7242EFE' },
    @{ n = 'GlobalSign Root CA - R3';                            t = 'D69B561148F01C77C54578C10926DF5B856976AD' },
    @{ n = 'GlobalSign Root R46';                                t = '53A2B04BCA6BD645E6398A8EC40DD2BF77C3A290' },
    @{ n = 'GTS Root R1 (Google)';                               t = 'E1C950E6EF22F84C5645728B922060D7D5A7A3E8' },
    @{ n = 'Microsoft RSA Root Certificate Authority 2017';      t = '73A5E64A3BFF8316FF0EDCCC618A906E4EAE4D74' },
    @{ n = 'Microsoft ECC Root Certificate Authority 2017';      t = '999A64C37FF47D9FAB95F14769891460EEC4C3C5' },
    @{ n = 'Go Daddy Root Certificate Authority - G2';           t = '47BEABC922EAE80E78783462A79F45C254FDE68B' },
    @{ n = 'Starfield Root Certificate Authority - G2';          t = 'B51C067CEE2B0C3DF855AB2D92F4FE39D4E70F0E' },
    @{ n = 'Entrust Root Certification Authority - G2';          t = '8CF427FD790C3AD166068DE81E57EFBB932272D4' },
    @{ n = 'Certum Trusted Network CA';                          t = '07E032E020B72C3F192F0628A2593A19A70F069E' },
    @{ n = 'IdenTrust Commercial Root CA 1';                     t = 'DF717EAA4AD94EC9558499602D48DE5FBCF03A25' }
)

function Find-Cert( $thumb ) {
    foreach ( $store in @( 'LocalMachine', 'CurrentUser' ) ) {
        $hit = Get-ChildItem "Cert:\$store\Root" | Where-Object { $_.Thumbprint -eq $thumb }
        if ( $hit ) { return $hit[0] }
    }
    return $null
}

$bytes   = New-Object System.Collections.Generic.List[byte]
$found   = 0
$missing = @()
foreach ( $root in $roots ) {
    $cert = Find-Cert $root.t
    if ( $cert -eq $null ) {
        $missing += $root.n
        Write-Output ( "  MISSING  {0}" -f $root.n )
        continue
    }
    $der = $cert.RawData
    $bytes.AddRange( $der )
    $found++
    Write-Output ( "  {0,6}  {1}" -f $der.Length, $root.n )
}

$full = [IO.Path]::GetFullPath( $Out )
[IO.File]::WriteAllBytes( $full, $bytes.ToArray() )

Write-Output ''
Write-Output ( "{0} roots, {1} bytes -> {2}" -f $found, $bytes.Count, $full )
if ( $missing.Count -gt 0 ) {
    Write-Output ( "WARNING: {0} root(s) not in this machine's store, left out:" -f $missing.Count )
    $missing | ForEach-Object { Write-Output "  - $_" }
}
