/* ==========================================================================
 * SMBSIGN.H - AES-CMAC (RFC 4493), SMB 3.x's packet-signing MAC.
 * ==========================================================================
 * SMB 2.0.2 / 2.1 sign with HMAC-SHA256 (already in the TLS port); SMB 3.x
 * sign with AES-128-CMAC over the message with the signature field zeroed.
 * This is just the MAC; the SMB2 engine zeroes the field, runs it, and fills
 * the 16-byte Signature in.  Uses the TLS port's AES-128 block cipher.
 * ========================================================================== */
#ifndef PMDOS_SMBSIGN_H
#define PMDOS_SMBSIGN_H

/* AES-128-CMAC of 'msg' (len bytes, may be 0) under the 16-byte key. */
void aes_cmac( const unsigned char key[16], const unsigned char *msg, int len,
               unsigned char mac[16] );

/* The SMB3 key-derivation function (SP800-108 counter mode, HMAC-SHA256,
   L = 128 bits): out = KDF( key, label, context ).  'label' is passed with
   its own trailing NUL included in labellen; the KDF adds the SP800-108 0x00
   separator itself.  Needs the TLS port's hmac_sha256. */
void smb3_kdf( const unsigned char key[16],
               const unsigned char *label, int labellen,
               const unsigned char *context, int contextlen,
               unsigned char out[16] );

#endif /* PMDOS_SMBSIGN_H */
