;=====================================================================
; REDIR.INC - the kernel's view of the NDTF_REDIR request block
;
; The assembly face of INC\REDIR.H: the REDIR_* functions and the
; offsets into the request block the net-drive file branches fill and
; hand to the redirector driver (EBX = the function, EDI -> the block,
; ECX = NR_SIZE).  Every number here is REDIR.H's; a change there is a
; change here, and NR_SIZE is asserted against the fields below.
;
; Included by DOSMAIN.ASM with the other INC files.
;=====================================================================

; THE FUNCTIONS - EBX
REDIR_MOUNT     EQU     1
REDIR_UMOUNT    EQU     2
REDIR_FINDFIRST EQU     3
REDIR_FINDNEXT  EQU     4
REDIR_OPEN      EQU     5
REDIR_READ      EQU     6
REDIR_WRITE     EQU     7
REDIR_CLOSE     EQU     8
REDIR_INFO      EQU     9
REDIR_DELETE    EQU     10
REDIR_MKDIR     EQU     11
REDIR_RMDIR     EQU     12
REDIR_RENAME    EQU     13              ; rename/move: NR_PATH -> NR_SHARE

; The WfW bridge (PMDOS.386) ORs REDIR_RESUME into the op and polls the
; driver, which runs the op on a worker fibre that yields when a socket
; wait has no data; REDIR_PENDING in NR_STATUS means "call again".  See
; INC\REDIR.H and DRV\SMBFS\DRVFIB.ASM.
REDIR_RESUME    EQU     1000h
REDIR_PENDING   EQU     0FFh

; THE REQUEST BLOCK - offsets, matching the redir_req struct in REDIR.H
NR_OP          EQU     0
NR_DRIVE       EQU     4
NR_HANDLE      EQU     8
NR_STATUS      EQU     12
NR_POSLO       EQU     16
NR_POSHI       EQU     20
NR_COUNT       EQU     24
NR_RESULT      EQU     28
NR_SIZELO      EQU     32
NR_SIZEHI      EQU     36
NR_ATTR        EQU     40
NR_TIMEDATE    EQU     44
NR_BUFPTR      EQU     48
NR_GEN         EQU     52
NR_FREECLUS    EQU     56
NR_SPARE       EQU     60
NR_PATH        EQU     64              ; char[260]
NR_PATTERN     EQU     324             ; char[16]
NR_NAME        EQU     340             ; char[16]
NR_SERVER      EQU     356             ; char[64]
NR_SHARE       EQU     420             ; char[80]
NR_USER        EQU     500             ; char[64]
NR_PASS        EQU     564             ; char[64]
NR_DOMAIN      EQU     628             ; char[64]
NR_SIZE        EQU     692

; the open-file handle the driver keeps per net drive is stored in the
; SFT's cluster field (SFTF_NET says so); these name what overloads what
NET_PATH_MAX    EQU     260             ; NR_PATH capacity, with the NUL

; 21h/F1h AL=27h NET USE - the sub-functions (EAX bits 16-23) and the
; netuse_req a program hands in at EDI, matching REDIR.H
NETUSE_MAP      EQU     0
NETUSE_UNMAP    EQU     1
NETUSE_QUERY    EQU     2
NUF_INUSE       EQU     01h
NUF_SMB1        EQU     02h

NU_DRIVE        EQU     0
NU_STATUS       EQU     4
NU_FLAGS        EQU     8
NU_SERVER       EQU     12              ; char[64]
NU_SHARE        EQU     76              ; char[80]
NU_USER         EQU     156             ; char[64]
NU_PASS         EQU     220             ; char[64]
NU_DOMAIN       EQU     284             ; char[64]
NU_SIZE         EQU     348
