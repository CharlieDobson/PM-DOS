/*
 * DRVSOCK.H - the SMBFS driver's socket shim (see DRVSOCK.C)
 *
 * The TCP conversation the SMB2 engine needs, over NDS_SOCKCALL instead
 * of INT 21h.  Timeouts are in milliseconds.
 */
#ifndef DRVSOCK_H
#define DRVSOCK_H

#include "ndrv.h"

int  dsock_up( u32 ms );                         /* 1 once the stack has an address */
u32  dsock_resolve( const char *name, u32 ms );  /* an address, or 0 */
int  dsock_open( u32 type );                     /* a handle (>=1), or 0 */
void dsock_close( int handle );
int  dsock_connect( int handle, u32 ip, u16 port, u32 ms );   /* 1 established */
int  dsock_send( int handle, const void *data, u32 len, u32 ms );   /* bytes sent */
int  dsock_recvwait( int handle, void *buf, u32 max, u32 ms );      /* bytes; 0 closed/timeout */

/* Called from each wait loop when it has no progress to make: on the WfW
   bridge's worker fibre it yields the slice back to the poll (so Windows
   runs and the tick pumps the parked stack); run natively it does nothing
   and the loop spins as before.  Defined in SMBFS.C. */
void dsock_idle( void );

#endif /* DRVSOCK_H */
