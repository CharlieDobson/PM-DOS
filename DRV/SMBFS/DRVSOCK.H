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
void dsock_nbname( char *out );                  /* TCPIP.SYS /NAME= into out[16], if one */
u32  dsock_resolve( const char *name, u32 ms );  /* an address, or 0 */
int  dsock_open( u32 type );                     /* a handle (>=1), or 0 */
void dsock_close( int handle );
int  dsock_connect( int handle, u32 ip, u16 port, u32 ms );   /* 1 established */
int  dsock_send( int handle, const void *data, u32 len, u32 ms );   /* bytes sent */
int  dsock_recvwait( int handle, void *buf, u32 max, u32 ms );      /* bytes; 0 closed/timeout */

/* NetBIOS over NetBEUI, through NETBEUI.SYS (INC\NETBIOS.H) */
int  dnb_here( char *name );                     /* 1 when NETBEUI.SYS has its name, into name[16] */
u32  dnb_call( const char *name, u8 suffix, u32 ms );   /* a session handle, or 0 */
void dnb_hangup( u32 handle );
int  dnb_send( u32 handle, const void *data, u32 len, u32 ms );    /* 1: the message is going */
u32  dnb_wait( u32 handle, u32 ms );             /* the next message's length; 0 none */
u32  dnb_recv( u32 handle, void *out, u32 max ); /* the next part of it */

/* Called from each wait loop when it has no progress to make: on the WfW
   bridge's worker fibre it yields the slice back to the poll (so Windows
   runs and the tick pumps the parked stack); run natively it does nothing
   and the loop spins as before.  Defined in SMBFS.C. */
void dsock_idle( void );

#endif /* DRVSOCK_H */
